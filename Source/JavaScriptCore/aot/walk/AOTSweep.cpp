/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSweep.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTRepresentation.h"
#include "AOTSort.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint8_t neededMarkOfSweep = static_cast<uint8_t>(SweepMark::IsNeeded);
static constexpr uint8_t leavesOwedErrorMarkOfSweep = static_cast<uint8_t>(SweepMark::LeavesOwedError);
static constexpr uint8_t pollsForNothingMarkOfSweep = static_cast<uint8_t>(SweepMark::PollsForNothing);
// The parameters of reached blocks, and each operation that the way has come by. Whatever is found needed with its bit set goes through the worklist. sweepBackward() takes the bit away again.
static constexpr uint8_t passedByMarkOfSweep = 1 << 6;
static_assert(!OptionSet<SweepMark> { SweepMark::IsNeeded, SweepMark::ReleasesOperand, SweepMark::LaterBarrierCovers, SweepMark::LeavesOwedError, SweepMark::PollsForNothing, SweepMark::LaterFenceCovers, SweepMark::OwnerIsYoung }.containsAny(OptionSet<SweepMark>::fromRaw(passedByMarkOfSweep)));

// Kept whatever their traits say: coverage counters are never removed, a loop without a poll cannot be interrupted, and what was decided by a proof has left nobody else who looks at the value.
static constexpr OpcodeSet operationsThatSweepNeverDrops { Opcode::LoopHint, Opcode::CountExecution, Opcode::CheckProof };

// Each speaks of a cell and does nothing for the program: where nothing else needs the cell, neither is made.
static constexpr OpcodeSet operationsThatSweepKeepsForTheirOperand { Opcode::ReleaseHint, Opcode::MarkEnvironmentEscaped, Opcode::ReleaseEnvironment, Opcode::ClearFrameCell };
static_assert(OperandIndex::ReleaseHint::value == OperandIndex::MarkEnvironmentEscaped::environment && OperandIndex::ReleaseHint::value == OperandIndex::ReleaseEnvironment::environment);
static_assert(OperandIndex::ReleaseHint::value == OperandIndex::ClearFrameCell::cell);

static constexpr uint8_t isKeptForOperandInRowOfSweep = 1 << 0;
static constexpr uint8_t isNeverDroppedInRowOfSweep = 1 << 1;
static constexpr uint8_t writesOnlyItsAnswerInRowOfSweep = 1 << 2;
static constexpr uint8_t hasNoResultInRowOfSweep = 1 << 3;
static constexpr std::array<uint8_t, numberOfEnumerators<Opcode>> rowsOfOpcodesInSweep = [] {
    std::array<uint8_t, numberOfEnumerators<Opcode>> rows { };
    for (Opcode opcode : allMembersOf<Opcode>) {
        uint8_t row = 0;
        if (operationsThatSweepKeepsForTheirOperand.contains(opcode))
            row |= isKeptForOperandInRowOfSweep;
        if (operationsThatSweepNeverDrops.contains(opcode))
            row |= isNeverDroppedInRowOfSweep;
        if (writesOnlyItsAnswerOf(opcode))
            row |= writesOnlyItsAnswerInRowOfSweep;
        if (resultRuleOf(opcode) == ResultRule::None)
            row |= hasNoResultInRowOfSweep;
        rows[static_cast<unsigned>(opcode)] = row;
    }
    return rows;
}();

namespace {

struct BlockOnSearchPathOfSweep {
    BlockID block;
    uint32_t nextEdge { 0 };
};

} // anonymous namespace

// A Narrowed emits no code that reads its test, and the branch on the test keeps the test needed.
template<typename Functor>
static void forEachOperandNeededBySweep(const Operation& operation, const Functor& functor)
{
    if (operation.opcode == Opcode::Narrowed) {
        functor(operation.operand(OperandIndex::Narrowed::value));
        return;
    }
    operation.forEachChild(functor);
}

// The row of a function or getter of the host says what the call changes, and nobody checks what the row leaves out: the state may be read through another object, or be ambient.
// A built-in of the language works on its operands and on the heaps that its row names, so a call of one goes by its traits.
static bool callsBuiltinOfHostInSweep(const BuiltinTable& table, const Operation& callBuiltin)
{
    return table.at(table.at(callBuiltin.overload()).builtin).origin != BuiltinOrigin::Engine;
}

static std::tuple<uint32_t, FactKind, FactSubject::Kind, uint32_t, uint32_t> sortKeyOfFactUseInSweep(const FactUse& use)
{
    return { use.record.index(), use.kind, use.subjectKind, use.subject, use.index };
}

SweepTotals Sweep::run(const SweepInputs& inputs, Code& code)
{
    m_inputs = &inputs;
    m_code = &code;
    // operationsSwept rises each time a loop visits a record that the walk appended to a block, so a further pass over Code shows in it. Parameters and constants are not among OperationsBuilt, and do not count here.
    m_totals = { };

    uint32_t numberOfValues = code.numberOfValues();
    uint32_t numberOfBlocks = code.numberOfBlocks();
    code.m_marks.fill(0, numberOfValues);
    for (PollWithFate listed : code.pollsWithFates())
        code.m_marks[listed.poll.index()] |= listed.fate == FateOfPoll::LeavesHandler ? leavesOwedErrorMarkOfSweep : pollsForNothingMarkOfSweep;
    code.m_suspensions.shrink(0);
    code.m_liveValues.shrink(0);
    code.m_reachedBlocks.clearAll();
    code.m_reachedBlocks.ensureSize(numberOfBlocks);
    code.m_blocksWithoutThrows.clearAll();
    code.m_blocksWithoutThrows.ensureSize(numberOfBlocks);
    // Set first, so that Code::handlerOf() and Code::forEachEdgeOf() skip the handler edges that are given up, in every step below.
    code.m_isSwept = true;
    m_worklist.shrink(0);
    m_blocksToReach.shrink(0);
    m_reachedInPostorder.shrink(0);
    m_holdsBackStores = false;
    m_hasListedIncomingEdges = false;
    m_hasGivenUpHandlers = false;

    sweepBackward();

    m_inputs = nullptr;
    m_code = nullptr;
    return m_totals;
}

// Afterward, the edges that lead to block b are m_incomingEdges[m_firstIncomingEdge[b] .. m_firstIncomingEdge[b + 1]), whether or not anything takes them.
void Sweep::listIncomingEdges()
{
    const Code& code = *m_code;
    uint32_t numberOfBlocks = code.numberOfBlocks();
    uint32_t numberOfEdges = code.numberOfEdges();
    m_hasListedIncomingEdges = true;
    m_firstIncomingEdge.fill(0, numberOfBlocks + 1);
    for (uint32_t index = 0; index < numberOfEdges; ++index) {
        if (BlockID to = code.at(EdgeID(index)).to)
            ++m_firstIncomingEdge[to.index()];
    }

    uint32_t numberOfIncomingEdges = 0;
    for (uint32_t index = 0; index <= numberOfBlocks; ++index) {
        numberOfIncomingEdges += m_firstIncomingEdge[index];
        m_firstIncomingEdge[index] = numberOfIncomingEdges;
    }
    m_incomingEdges.fill(EdgeID(), numberOfIncomingEdges);
    for (uint32_t index = numberOfEdges; index--;) {
        if (BlockID to = code.at(EdgeID(index)).to)
            m_incomingEdges[--m_firstIncomingEdge[to.index()]] = EdgeID(index);
    }
}

// Afterward, the lists hold the edges from reached blocks alone, without the handler edges that were given up.
void Sweep::forgetEdgesThatAreNotTaken()
{
    uint32_t numberOfBlocks = m_code->numberOfBlocks();
    uint32_t numberKept = 0;
    uint32_t next = 0;
    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        uint32_t end = m_firstIncomingEdge[index + 1];
        m_firstIncomingEdge[index] = numberKept;
        for (; next < end; ++next) {
            if (isTaken(m_incomingEdges[next]))
                m_incomingEdges[numberKept++] = m_incomingEdges[next];
        }
    }
    m_firstIncomingEdge[numberOfBlocks] = numberKept;
    m_incomingEdges.shrink(numberKept);
}

// A unit with handlers sets its frame up at entry and traps nowhere.
void Sweep::forgetHandlersThatAreGone()
{
    Code& code = *m_code;
    if (!code.m_hasHandlers)
        return;
    code.m_hasHandlers = std::ranges::any_of(m_reachedInPostorder, [&](BlockID block) {
        return code.handlerOf(block) || code.at(block).isHandlerBlock();
    });
}

bool Sweep::isTaken(EdgeID id) const
{
    const Code& code = *m_code;
    BlockID from = code.at(id).from;
    return code.m_reachedBlocks.quickGet(from.index()) && (code.at(from).handler != id || !code.m_blocksWithoutThrows.quickGet(from.index()));
}

void Sweep::reach(BlockID start)
{
    Code& code = *m_code;
    if (code.m_reachedBlocks.quickSet(start.index()))
        return;

    Vector<BlockOnSearchPathOfSweep, 32> path;
    scanRootsOf(start);
    BlockID from = start;
    uint32_t nextEdge = 0;
    for (;;) {
        const Terminator& terminator = code.at(from).terminator;
        uint32_t numberOfEdges = terminator.numberOfEdges;
        BlockID below;
        while (nextEdge < numberOfEdges) {
            const Edge& edge = code.at(terminator.edge(nextEdge++));
            BlockID to = edge.to;
            RELEASE_ASSERT(to);
            needArgumentsOn(edge);
            if (code.m_reachedBlocks.quickSet(to.index()))
                continue;
            scanRootsOf(to);
            if (code.at(to).terminator.numberOfEdges) {
                below = to;
                break;
            }
            m_reachedInPostorder.append(to);
        }
        if (below) {
            path.append({ from, nextEdge });
            from = below;
            nextEdge = 0;
            continue;
        }
        m_reachedInPostorder.append(from);
        if (path.isEmpty())
            return;
        BlockOnSearchPathOfSweep above = path.takeLast();
        from = above.block;
        nextEdge = above.nextEdge;
    }
}

ALWAYS_INLINE void Sweep::scanRootsOf(BlockID id)
{
    Code& code = *m_code;
    const Block& block = code.at(id);
    RELEASE_ASSERT(block.terminator.isSet);
    for (ValueID parameter : block.parameters()) {
        uint8_t& marks = code.m_marks[parameter.index()];
        if (marks & passedByMarkOfSweep)
            continue;
        marks |= passedByMarkOfSweep;
        if (marks & neededMarkOfSweep)
            m_worklist.append(parameter);
    }
    if (block.handler)
        giveUpHandlerOf(id);

    block.terminator.forEachChild([&](ValueID operand) {
        need(operand);
        countUse(operand, ValueID());
    });
    if (block.subject) {
        need(block.subject);
        countUse(block.subject, ValueID());
    }
    // visitOperationsOf() finds the roots among the operations.
}

void Sweep::need(ValueID value)
{
    uint8_t& marks = m_code->m_marks[value.index()];
    if (marks & neededMarkOfSweep)
        return;
    marks |= neededMarkOfSweep;
    // The way has yet to come by it. A constant, by which it never comes, waits for needOperandsOfWhatWayDoesNotComeBy().
    if (!(marks & passedByMarkOfSweep))
        return;
    m_worklist.append(value);
}

void Sweep::needArgumentsFor(ValueID parameter)
{
    const Code& code = *m_code;
    BlockID block = code.blockOf(parameter);
    RELEASE_ASSERT(block);
    std::span<const ValueID> parameters = code.at(block).parameters();
    RELEASE_ASSERT(!parameters.empty());

    size_t position = parameter.index() - parameters.front().index();
    if (position >= parameters.size() || parameters[position] != parameter)
        position = std::ranges::find(parameters, parameter) - parameters.begin();
    RELEASE_ASSERT(position < parameters.size());

    if (!code.at(block).firstEdgeIn())
        return;
    if (!m_hasListedIncomingEdges)
        listIncomingEdges();

    // Parameters past the edge's arguments receive the terminator's declared results, and a terminator is never dropped.
    // An edge that begins to be taken later brings its argument then: needArgumentsOn().
    for (uint32_t index = m_firstIncomingEdge[block.index()]; index < m_firstIncomingEdge[block.index() + 1]; ++index) {
        if (!isTaken(m_incomingEdges[index]))
            continue;
        std::span<const ValueID> arguments = code.at(m_incomingEdges[index]).arguments();
        if (position < arguments.size() && arguments[position])
            need(arguments[position]);
    }
}

void Sweep::needArgumentsOn(EdgeID id)
{
    needArgumentsOn(m_code->at(id));
}

void Sweep::needArgumentsOn(const Edge& edge)
{
    const Code& code = *m_code;
    std::span<const ValueID> arguments = edge.arguments();
    if (arguments.empty())
        return;
    m_blocksThatHandOverArguments.quickSet(edge.from.index());
    std::span<const ValueID> parameters = code.at(edge.to).parameters();
    size_t numberOfPairs = std::min(parameters.size(), arguments.size());
    for (size_t position = 0; position < numberOfPairs; ++position) {
        if (arguments[position] && code.isNeeded(parameters[position]))
            need(arguments[position]);
    }
}

bool Sweep::isRoot(const Operation& operation) const
{
    return staysInSweptCodeByItself(m_inputs->program, operation);
}

bool staysInSweptCodeByItself(const Program& program, const Operation& operation)
{
    uint8_t row = rowsOfOpcodesInSweep[static_cast<unsigned>(operation.opcode)];
    // A hint keeps nothing alive: settleOperationsKeptForTheirOperands() keeps it only if its operand is needed.
    if (row & isKeptForOperandInRowOfSweep)
        return false;
    if (row & isNeverDroppedInRowOfSweep)
        return true;
    if (operation.opcode == Opcode::CallBuiltin && callsBuiltinOfHostInSweep(program.builtins(), operation))
        return true;
    // Its write is seen through its answer alone. Where it tests its own guess the traits are those of the generic operation, which may run code.
    EffectTraits traits = operation.traits();
    if ((row & writesOnlyItsAnswerInRowOfSweep) && !traits.mayThrowByRule() && traits.alwaysCompletes())
        return false;
    if (!neitherThrowsNorRunsCode(traits))
        return true;
    // An operation without a result is never used, so being unused says nothing about it: FinishBirth writes a word that no JavaScript read sees but the engine reads.
    return row & hasNoResultInRowOfSweep;
}

void Sweep::sweepBackward()
{
    Code& code = *m_code;
    uint32_t numberOfValues = code.numberOfValues();
    m_operationsNeededLate.shrink(0);
    m_operationsKeptForOperands.shrink(0);
    m_numberOfOperationsInOrder = 0;
    code.m_order.shrink(0);
    code.m_useCounts.fill(0, numberOfValues);
    code.m_onlyUsers.fill(ValueID(), numberOfValues);
    for (uint32_t index = 0; index < code.numberOfBlocks(); ++index) {
        code.m_blocks[index].m_firstInOrder = 0;
        code.m_blocks[index].m_numberInOrder = 0;
    }
    m_looksForCoveredBarriers = beginLookingForCoveredBarriers();
    m_looksForCoveredFences = beginLookingForCoveredFences();
    m_coverageIsOutOfDate = false;
    beginFollowingLiveness();
    m_blocksThatHandOverArguments.clearAll();
    m_blocksThatHandOverArguments.ensureSize(code.numberOfBlocks());

    for (const EntryOfCode& entry : code.entries())
        reach(entry.block);
    size_t next = 0;
    do {
        settleWhatIsNeeded();
        // The list grows where something needed turns out to throw to a handler block.
        for (; next < m_reachedInPostorder.size(); ++next) {
            visitOperationsOf(m_reachedInPostorder[next]);
            if (!m_worklist.isEmpty() || !m_blocksToReach.isEmpty())
                settleWhatIsNeeded();
        }
        needOperandsOfWhatWayDoesNotComeBy();
    } while (!m_worklist.isEmpty());
    for (uint8_t& marks : code.m_marks)
        marks &= static_cast<uint8_t>(~passedByMarkOfSweep);
    // From here on finishLiveness() alone goes by the lists of incoming edges.
    if (m_followsLiveness) {
        if (!m_hasListedIncomingEdges)
            listIncomingEdges();
        forgetEdgesThatAreNotTaken();
    }
    forgetHandlersThatAreGone();

    settleOperationsKeptForTheirOperands();
    putOperationsNeededLateInOrder();
    m_totals.operationsDropped -= m_numberOfOperationsInOrder;
    countUsesAtEndsOfBlocks();
    if (m_coverageIsOutOfDate)
        lookForCoverageAgain();
    sortFactUses();
    finishLiveness();
}

ALWAYS_INLINE void Sweep::visitOperationsOf(BlockID id)
{
    Code& code = *m_code;
    Block& block = code.m_blocks[id.index()];
    uint32_t firstInOrder = static_cast<uint32_t>(code.m_order.size());
    block.m_firstInOrder = firstInOrder;

    const bool looksForCoveredBarriers = m_looksForCoveredBarriers;
    const bool looksForCoveredFences = m_looksForCoveredFences;
    if (looksForCoveredBarriers && !m_coveredAtHeads.isEmpty())
        beginCoverageAtEndOf(id);
    if (looksForCoveredFences) {
        if (m_laterFenceCoversAtSomeHead)
            beginFenceCoverageAtEndOf(id);
        else
            m_laterFenceCovers = false;
    }
    uint32_t numberOfOperationsVisited = 0;
    ValueID previous;
    for (ValueID value = block.m_last; value; value = previous) {
        const Operation& operation = code.at(value);
        previous = operation.previous;
        Opcode opcode = operation.opcode;
        if (opcode == Opcode::Parameter)
            continue;
        ++numberOfOperationsVisited;

        uint8_t& marks = code.m_marks[value.index()];
        marks |= passedByMarkOfSweep;
        bool isNeeded = marks & neededMarkOfSweep;
        if (!isNeeded) {
            if (rowsOfOpcodesInSweep[static_cast<unsigned>(opcode)] & isKeptForOperandInRowOfSweep) {
                if (!noteOperationKeptForItsOperand(id, value, operation))
                    continue;
            } else {
                if ((marks & pollsForNothingMarkOfSweep) || !isRoot(operation))
                    continue;
                EffectTraits traits = operation.traits();
                ValueID object;
                if (!traits.mayBeTerminated() && !traits.mayRunOtherCode() && !traits.mayWriteOutsideObjects() && isStoreIntoOwnObject(operation, object) && !code.isNeeded(object)) {
                    holdBackUntilNeeded(value, object);
                    continue;
                }
                marks |= neededMarkOfSweep;
                isNeeded = true;
            }
        }
        code.m_order.append(value);
        if (isNeeded)
            noteNeededOperation(value, operation);
        if (looksForCoveredBarriers && (!m_coveredOwners.isEmpty() || sweptOperationsWithOwnerBarriers.contains(opcode)))
            lookForCoveredBarrier(value, operation);
        if (looksForCoveredBarriers && (!m_storesThatAwaitBirth.isEmpty() || sweptOperationsWithOwnerBarriers.contains(opcode)))
            lookForBirthOfOwner(value, operation);
        if (looksForCoveredFences && (m_laterFenceCovers || sweptAllocationsWithFences.contains(opcode)))
            lookForCoveredFence(value, operation);
    }
    m_totals.operationsSwept += numberOfOperationsVisited;
    // Until sweepBackward() takes off what it keeps.
    m_totals.operationsDropped += numberOfOperationsVisited;
    if (looksForCoveredBarriers && !m_coveredOwners.isEmpty())
        endCoverageAtHeadOf(id);
    if (!m_storesThatAwaitBirth.isEmpty())
        forgetStoresThatAwaitBirth();
    if (looksForCoveredFences && m_laterFenceCovers)
        endFenceCoverageAtHeadOf(id);
    uint32_t numberInOrder = static_cast<uint32_t>(code.m_order.size()) - firstInOrder;
    if (numberInOrder > 1)
        std::ranges::reverse(code.m_order.mutableSpan().subspan(firstInOrder));
    block.m_numberInOrder = numberInOrder;
    m_numberOfOperationsInOrder += numberInOrder;
}

ALWAYS_INLINE void Sweep::noteNeededOperation(ValueID value, const Operation& operation)
{
    Code& code = *m_code;
    // A poll that leaves an owed error owed throws nothing that a handler could catch.
    if (m_hasGivenUpHandlers && code.m_blocksWithoutThrows.quickGet(operation.blockIndex) && !(code.m_marks[value.index()] & leavesOwedErrorMarkOfSweep))
        keepHandlerIfThrows(operation);
    if (m_holdsBackStores)
        needStoresHeldBackFor(value);
    forEachOperandNeededBySweep(operation, [&](ValueID operand) {
        need(operand);
        countUse(operand, value);
    });
    if (m_followsLiveness)
        noteNeededOperationForLiveness(value, operation);
}

// Afterward nothing waits: each parameter that is needed has asked for its arguments, each handler block that something needed throws to is listed, and each operation that was found needed behind the way has been visited.
void Sweep::settleWhatIsNeeded()
{
    Code& code = *m_code;
    for (;;) {
        while (!m_blocksToReach.isEmpty())
            reach(m_blocksToReach.takeLast());
        if (m_worklist.isEmpty())
            return;

        ValueID value = m_worklist.takeLast();
        const Operation& operation = code.at(value);
        if (operation.opcode == Opcode::Parameter) {
            needArgumentsFor(value);
            continue;
        }
        ++m_totals.operationsSwept;
        if (!mayBeKeptForItsOperand(operation))
            m_operationsNeededLate.append({ operation.block(), value });
        noteNeededOperation(value, operation);
        m_coverageIsOutOfDate = m_coverageIsOutOfDate || mayChangeWhatIsCovered(operation);
    }
}

bool Sweep::mayBeKeptForItsOperand(const Operation& operation) const
{
    if (!operationsThatSweepKeepsForTheirOperand.contains(operation.opcode))
        return false;
    ValueID operand = operation.operand(OperandIndex::ReleaseHint::value);
    return !m_code->isConstant(operand) && holdsCells(m_code->at(operand).representation);
}

bool Sweep::noteOperationKeptForItsOperand(BlockID block, ValueID id, const Operation& operation)
{
    bool mayBeKept = mayBeKeptForItsOperand(operation);
    bool isHint = operation.opcode == Opcode::ReleaseHint;
    if (!mayBeKept && !isHint)
        return false;
    ValueID operand = operation.operand(OperandIndex::ReleaseHint::value);
    bool isBehindCopiedBody = isHint && operation.isBehindCopiedBody();
    m_operationsKeptForOperands.append({ block, id, operand, operation.opcode, mayBeKept, isBehindCopiedBody });
    if (mayBeKept && isBehindCopiedBody)
        noteValueHeldByCopiedBody(operand);
    return mayBeKept;
}

// Each has its one operand and needs nothing: it stays where something else needs the operand.
void Sweep::settleOperationsKeptForTheirOperands()
{
    Code& code = *m_code;
    Vector<BlockID, 8> blocksWithGaps;
    for (const OperationKeptForOperand& kept : m_operationsKeptForOperands) {
        // Something uses what it gives, so it has been noted like any other.
        if (!kept.standsInOrder || code.isNeeded(kept.operation))
            continue;
        if (code.isNeeded(kept.operand)) {
            code.m_marks[kept.operation.index()] |= neededMarkOfSweep;
            countUse(kept.operand, kept.operation);
            if (m_followsLiveness && kept.opcode != Opcode::ReleaseHint && kept.opcode != Opcode::ReleaseEnvironment) {
                ++m_totals.operationsSwept;
                noteNeededOperationForLiveness(kept.operation, code.at(kept.operation));
            }
            continue;
        }
        if (!m_coverageIsOutOfDate) {
            ++m_totals.operationsSwept;
            m_coverageIsOutOfDate = mayChangeWhatIsCovered(code.at(kept.operation));
        }
        if (blocksWithGaps.isEmpty() || blocksWithGaps.last() != kept.block)
            blocksWithGaps.append(kept.block);
    }

    for (BlockID id : blocksWithGaps) {
        Block& block = code.m_blocks[id.index()];
        std::span<ValueID> order = code.m_order.mutableSpan().subspan(block.m_firstInOrder, block.m_numberInOrder);
        uint32_t numberKept = 0;
        for (ValueID value : order) {
            if (code.isNeeded(value))
                order[numberKept++] = value;
        }
        m_numberOfOperationsInOrder -= block.m_numberInOrder - numberKept;
        block.m_numberInOrder = numberKept;
    }
}

// Records of one block ascend in its order. A block that gains some gets a new stretch of Code::m_order, which nobody reads but block by block.
void Sweep::putOperationsNeededLateInOrder()
{
    Code& code = *m_code;
    if (m_operationsNeededLate.isEmpty())
        return;
    AOT::sort(m_operationsNeededLate, { }, [](const OperationInBlock& late) {
        return std::tuple(late.block, late.operation);
    });
    for (size_t first = 0, end = 0; first < m_operationsNeededLate.size(); first = end) {
        BlockID id = m_operationsNeededLate[first].block;
        while (end < m_operationsNeededLate.size() && m_operationsNeededLate[end].block == id)
            ++end;

        Block& block = code.m_blocks[id.index()];
        uint32_t numberInOrder = block.m_numberInOrder + static_cast<uint32_t>(end - first);
        uint32_t firstInOrder = static_cast<uint32_t>(code.m_order.size());
        code.m_order.grow(firstInOrder + numberInOrder);
        std::span<ValueID> order = code.m_order.mutableSpan();
        size_t early = block.m_firstInOrder;
        size_t endOfEarly = early + block.m_numberInOrder;
        size_t late = first;
        for (size_t index = firstInOrder; index < order.size(); ++index) {
            bool takesEarly = early < endOfEarly && (late == end || order[early] < m_operationsNeededLate[late].operation);
            order[index] = takesEarly ? order[early++] : m_operationsNeededLate[late++].operation;
        }
        block.m_firstInOrder = firstInOrder;
        block.m_numberInOrder = numberInOrder;
    }
    m_numberOfOperationsInOrder += static_cast<uint32_t>(m_operationsNeededLate.size());
}

// Every reached block has been visited. A constant lies in no block and is made of constants alone, so what it needs changes nothing for any operation, and the worklist stays empty.
void Sweep::needOperandsOfWhatWayDoesNotComeBy()
{
    Code& code = *m_code;
    ASSERT(m_worklist.isEmpty());
    Vector<ValueID, 16> notComeBy;
    std::span<const uint8_t> marks = code.m_marks.span();
    constexpr size_t marksInWord = sizeof(uint64_t);
    constexpr uint64_t neededMarkOfEach = 0x0101010101010101ull * neededMarkOfSweep;
    static_assert(neededMarkOfSweep == 1 && passedByMarkOfSweep == 1 << 6 && std::endian::native == std::endian::little);
    size_t index = 0;
    for (; index + marksInWord <= marks.size(); index += marksInWord) {
        uint64_t word;
        memcpy(&word, marks.subspan(index, marksInWord).data(), marksInWord);
        for (uint64_t found = word & ~(word >> 6) & neededMarkOfEach; found; found &= found - 1)
            notComeBy.append(ValueID(static_cast<uint32_t>(index + std::countr_zero(found) / 8)));
    }
    for (; index < marks.size(); ++index) {
        if ((marks[index] & (neededMarkOfSweep | passedByMarkOfSweep)) == neededMarkOfSweep)
            notComeBy.append(ValueID(static_cast<uint32_t>(index)));
    }
    while (!notComeBy.isEmpty()) {
        forEachOperandNeededBySweep(code.at(notComeBy.takeLast()), [&](ValueID operand) {
            if (code.isNeeded(operand))
                return;
            need(operand);
            if (!(marks[operand.index()] & passedByMarkOfSweep))
                notComeBy.append(operand);
        });
    }
}

// A null user stands for a terminator, an edge or a block's subject.
void Sweep::countUse(ValueID used, ValueID user)
{
    Code& code = *m_code;
    uint8_t& count = code.m_useCounts[used.index()];
    if (count == std::numeric_limits<uint8_t>::max())
        return;
    code.m_onlyUsers[used.index()] = count ? ValueID() : user;
    ++count;
}

// Over blocks and edges, once it is known which parameters are needed and which edges are taken.
void Sweep::countUsesAtEndsOfBlocks()
{
    Code& code = *m_code;
    bool notesParametersFed = code.hasLoops();
    code.m_parametersFed.fill(ValueID(), notesParametersFed ? code.numberOfValues() : 0);
    auto countUsesOn = [&](const Edge& edge) {
        std::span<const ValueID> arguments = edge.arguments();
        if (arguments.empty())
            return;
        std::span<const ValueID> parameters = code.at(edge.to).parameters();
        size_t numberOfPairs = std::min(parameters.size(), arguments.size());
        for (size_t position = 0; position < numberOfPairs; ++position) {
            if (!arguments[position] || !code.isNeeded(parameters[position]))
                continue;
            countUse(arguments[position], ValueID());
            if (!notesParametersFed)
                continue;
            // A record stands behind what it is made of, so a parameter in front of its argument is one that the edge leads back to.
            ValueID& fed = code.m_parametersFed[arguments[position].index()];
            if (!fed || parameters[position] < arguments[position])
                fed = parameters[position];
        }
    };
    for (BlockID id : m_reachedInPostorder) {
        if (!m_blocksThatHandOverArguments.quickGet(id.index()))
            continue;
        const Block& block = code.at(id);
        const Terminator& terminator = block.terminator;
        for (uint32_t index = 0; index < terminator.numberOfEdges; ++index)
            countUsesOn(code.at(terminator.edge(index)));
        if (block.handler && !code.m_blocksWithoutThrows.quickGet(id.index()))
            countUsesOn(code.at(block.handler));
    }
}

void Sweep::sortFactUses()
{
    Vector<FactUse>& uses = m_code->m_factUses;
    if (!std::ranges::is_sorted(uses, { }, sortKeyOfFactUseInSweep))
        AOT::stableSort(uses, { }, sortKeyOfFactUseInSweep);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
