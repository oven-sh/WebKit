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
#include "AOTInliningAcrossFiles.h"
#include "AOTRepresentation.h"
#include "AOTSort.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint8_t leavesOwedErrorMarkOfSweep = static_cast<uint8_t>(SweepMark::LeavesOwedError);
static constexpr uint8_t pollsForNothingMarkOfSweep = static_cast<uint8_t>(SweepMark::PollsForNothing);
static_assert(!OptionSet<SweepMark> { SweepMark::IsNeeded, SweepMark::ReleasesOperand, SweepMark::LaterBarrierCovers, SweepMark::LeavesOwedError, SweepMark::PollsForNothing, SweepMark::LaterFenceCovers, SweepMark::OwnerIsYoung }.containsAny(OptionSet<SweepMark>::fromRaw(passedByMarkOfSweep)));

static constexpr OpcodeSet operationsThatSweepNeverDrops { Opcode::LoopHint, Opcode::CountExecution, Opcode::CheckProof, Opcode::EndCellLifetime, Opcode::AskObjectAgain, Opcode::EnterBodyThatRunsOnce, Opcode::CallBuiltin };

static constexpr OpcodeSet operationsThatSweepKeepsForTheirOperand { Opcode::MarkEnvironmentEscaped, Opcode::ReleaseEnvironment };
static_assert(OperandIndex::MarkEnvironmentEscaped::environment == OperandIndex::ReleaseEnvironment::environment);

static constexpr uint8_t isKeptForOperandInRowOfSweep = 1 << 0;
static constexpr uint8_t isNeverDroppedInRowOfSweep = 1 << 1;
static constexpr uint8_t writesOnlyItsAnswerInRowOfSweep = 1 << 2;
static constexpr uint8_t hasNoResultInRowOfSweep = 1 << 3;
static constexpr uint8_t hasOwnerBarrierInRowOfSweep = 1 << 4;
static constexpr uint8_t isAllocationWithFenceInRowOfSweep = 1 << 5;
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
        if (sweptOperationsWithOwnerBarriers.contains(opcode))
            row |= hasOwnerBarrierInRowOfSweep;
        if (sweptAllocationsWithFences.contains(opcode))
            row |= isAllocationWithFenceInRowOfSweep;
        rows[static_cast<unsigned>(opcode)] = row;
    }
    return rows;
}();

static ALWAYS_INLINE uint8_t rowOfOpcodeInSweep(Opcode opcode)
{
    return rowsOfOpcodesInSweep.data()[static_cast<unsigned>(opcode)];
}

static std::tuple<uint32_t, FactKind, FactSubject::Kind, uint32_t, uint32_t> sortKeyOfFactUseInSweep(const FactUse& use)
{
    return { use.record.index(), use.kind, use.subjectKind, use.subject, use.index };
}

static ALWAYS_INLINE void needInSweep(uint8_t* marksOfValues, Vector<ValueID>& worklist, ValueID value)
{
    uint8_t& marks = marksOfValues[value.index()];
    if (marks & neededMarkOfSweep)
        return;
    marks |= neededMarkOfSweep;
    if (!(marks & passedByMarkOfSweep))
        return;
    worklist.append(value);
}

static ALWAYS_INLINE void countUseInSweep(uint8_t* useCounts, ValueID* onlyUsers, ValueID used, ValueID user)
{
    uint8_t& count = useCounts[used.index()];
    if (count == std::numeric_limits<uint8_t>::max())
        return;
    onlyUsers[used.index()] = count ? ValueID() : user;
    ++count;
}

Sweep::Sweep(RoomsOfSweep& rooms)
    : m_rooms(rooms)
    , m_statesOfBlocks(rooms.roomOfStatesOfBlocks.take())
    , m_worklist(rooms.roomOfWorklist.take())
    , m_blocksToReach(rooms.roomOfBlocksToReach.take())
    , m_firstIncomingEdge(rooms.roomOfFirstIncomingEdge.take())
    , m_incomingEdges(rooms.roomOfIncomingEdges.take())
    , m_searchPath(rooms.roomOfSearchPath.take())
    , m_reachedInPostorder(rooms.roomOfReachedInPostorder.take())
    , m_branchesThatDecideNothing(rooms.roomOfBranchesThatDecideNothing.take())
    , m_heldBack(rooms.roomOfHeldBack.take())
    , m_operandsWrittenByHeldBackStores(rooms.operandsWrittenByHeldBackStores)
    , m_opcodesOfAllocationsWhoseStoresAreHeldBack(rooms.opcodesOfAllocationsWhoseStoresAreHeldBack)
    , m_waitingStores(rooms.roomOfWaitingStores.take())
    , m_operationsNeededLate(rooms.roomOfOperationsNeededLate.take())
    , m_operationsKeptForOperands(rooms.roomOfOperationsKeptForOperands.take())
    , m_closuresThatMayBeLent(rooms.roomOfClosuresThatMayBeLent.take())
    , m_cellsThatMayBeLent(rooms.roomOfCellsThatMayBeLent.take())
    , m_birthsThatMayMove(rooms.roomOfBirthsThatMayMove.take())
    , m_environmentsHandedBack(rooms.roomOfEnvironmentsHandedBack.take())
    , m_namesOfCellsThatMayHoldEnvironments(rooms.roomOfNamesOfCellsThatMayHoldEnvironments.take())
    , m_environmentsFollowed(rooms.roomOfEnvironmentsFollowed.take())
    , m_blocksThatSuspend(rooms.roomOfBlocksThatSuspend.take())
    , m_blocksWithHints(rooms.roomOfBlocksWithHints.take())
    , m_lastReads(rooms.roomOfLastReads.take())
    , m_reads(rooms.roomOfReads.take())
    , m_lastReadersInOwnBlocks(rooms.roomOfLastReadersInOwnBlocks.take())
    , m_coveredAtHeads(rooms.roomOfCoveredAtHeads.take())
    , m_firstCoveredAtHead(rooms.roomOfFirstCoveredAtHead.take())
    , m_numberCoveredAtHead(rooms.roomOfNumberCoveredAtHead.take())
    , m_coveredOwners(rooms.roomOfCoveredOwners.take())
    , m_ownersCovered(rooms.roomOfOwnersCovered.take())
    , m_storesThatAwaitBirth(rooms.roomOfStoresThatAwaitBirth.take())
    , m_birthsAwaited(rooms.roomOfBirthsAwaited.take())
    , m_laterFenceCoversAtHeads(rooms.roomOfLaterFenceCoversAtHeads.take())
{
}

SweepTotals Sweep::run(const SweepInputs& inputs, Code& code)
{
    RELEASE_ASSERT(!m_code);
    m_inputs = &inputs;
    m_code = &code;

    uint32_t numberOfValues = code.numberOfValues();
    uint32_t numberOfBlocks = code.numberOfBlocks();
    code.m_marks.fill(0, numberOfValues);
    for (PollWithFate listed : code.pollsWithFates()) {
        code.m_marks[listed.poll.index()] |= listed.fate == FateOfPoll::LeavesHandler ? leavesOwedErrorMarkOfSweep : pollsForNothingMarkOfSweep;
        if (!listed.unlessKept)
            continue;
        code.m_marks[listed.unlessKept.index()] |= leavesOwedErrorMarkOfSweep;
        m_pollsThatLeaveUnlessKept.append(listed);
    }
    std::ranges::sort(m_pollsThatLeaveUnlessKept, { }, [](const PollWithFate& listed) { return listed.unlessKept.index(); });

    code.m_reachedBlocks.clearAll();
    code.m_reachedBlocks.ensureSize(numberOfBlocks);
    code.m_blocksWithoutThrows.clearAll();
    code.m_blocksWithoutThrows.ensureSize(numberOfBlocks);
    code.m_isSwept = true;

    sweepBackward();
    if (m_hasMovedBirths)
        return sweepAgainBehindMovedBirths(inputs, code);
    if (inputs.shouldDropEdge) [[unlikely]]
        dropTakenEdge();
    settlePollsThatLeaveUnlessRecordsAreKept();
    return m_totals;
}

void Sweep::keepPollsUnderHandlersBeside(ValueID kept)
{
    Code& code = *m_code;
    for (const PollWithFate& listed : std::ranges::equal_range(m_pollsThatLeaveUnlessKept, kept.index(), { }, [](const PollWithFate& listed) { return listed.unlessKept.index(); })) {
        uint8_t& marks = code.m_marks[listed.poll.index()];
        if (!(marks & leavesOwedErrorMarkOfSweep))
            continue;
        marks &= static_cast<uint8_t>(~leavesOwedErrorMarkOfSweep);
        const Operation& poll = code.at(listed.poll);
        if ((marks & passedByMarkOfSweep) && (m_states[poll.blockIndex] & (sweptBlockHasGivenUpHandler | sweptBlockNeedsNoHandler)) == sweptBlockHasGivenUpHandler)
            keepHandlerIfThrows(poll);
    }
}

void Sweep::settlePollsThatLeaveUnlessRecordsAreKept()
{
    Code& code = *m_code;
    for (const PollWithFate& listed : m_pollsThatLeaveUnlessKept) {
        RELEASE_ASSERT(!code.isNeeded(listed.unlessKept) || !(code.m_marks[listed.poll.index()] & leavesOwedErrorMarkOfSweep));
        code.m_marks[listed.unlessKept.index()] &= static_cast<uint8_t>(~leavesOwedErrorMarkOfSweep);
    }
}

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

ALWAYS_INLINE_INTO_OTHER_FILES bool Sweep::isTaken(EdgeID id, const Edge& edge) const
{
    return isSweptEdgeTaken(*m_code, m_states, id, edge);
}

void Sweep::listEdgesThatAreTaken()
{
    const Code& code = *m_code;
    uint32_t numberOfBlocks = code.numberOfBlocks();
    uint32_t numberOfEdges = code.numberOfEdges();
    m_hasListedIncomingEdges = true;
    m_firstIncomingEdge.fill(0, numberOfBlocks + 1);
    uint32_t* const firstIncomingEdge = m_firstIncomingEdge.mutableSpan().data();
    for (uint32_t index = 0; index < numberOfEdges; ++index) {
        const Edge& edge = code.at(EdgeID(index));
        if (edge.to && isTaken(EdgeID(index), edge))
            ++firstIncomingEdge[edge.to.index()];
    }

    uint32_t numberOfIncomingEdges = 0;
    for (uint32_t index = 0; index <= numberOfBlocks; ++index) {
        numberOfIncomingEdges += firstIncomingEdge[index];
        firstIncomingEdge[index] = numberOfIncomingEdges;
    }
    m_incomingEdges.fill(EdgeID(), numberOfIncomingEdges);
    EdgeID* const incomingEdges = m_incomingEdges.mutableSpan().data();
    for (uint32_t index = numberOfEdges; index--;) {
        const Edge& edge = code.at(EdgeID(index));
        if (edge.to && isTaken(EdgeID(index), edge))
            incomingEdges[--firstIncomingEdge[edge.to.index()]] = EdgeID(index);
    }
}

void Sweep::forgetEdgesThatAreNotTaken()
{
    const Code& code = *m_code;
    uint32_t numberOfBlocks = code.numberOfBlocks();
    uint32_t numberKept = 0;
    uint32_t next = 0;
    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        uint32_t end = m_firstIncomingEdge[index + 1];
        m_firstIncomingEdge[index] = numberKept;
        for (; next < end; ++next) {
            if (isTaken(m_incomingEdges[next], code.at(m_incomingEdges[next])))
                m_incomingEdges[numberKept++] = m_incomingEdges[next];
        }
    }
    m_firstIncomingEdge[numberOfBlocks] = numberKept;
    m_incomingEdges.shrink(numberKept);
}

void Sweep::ensureEdgesThatAreTakenAreListed()
{
    if (m_hasForgottenEdgesThatAreNotTaken)
        return;
    m_hasForgottenEdgesThatAreNotTaken = true;
    if (m_hasListedIncomingEdges)
        forgetEdgesThatAreNotTaken();
    else
        listEdgesThatAreTaken();
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Sweep::edgesAscendInto(BlockID id, const Block& block)
{
    if (m_blockThatEdgesAscendInto == id)
        return true;
    const Code& code = *m_code;
    EdgeID edge = block.firstEdgeIn();
    while (EdgeID next = code.at(edge).nextInto()) {
        if (next < edge)
            return false;
        edge = next;
    }
    m_blockThatEdgesAscendInto = id;
    return true;
}

void Sweep::forgetHandlersThatAreGone()
{
    Code& code = *m_code;
    code.m_hasHandlers = std::ranges::any_of(m_reachedInPostorder, [&](BlockID id) {
        const Block& block = code.at(id);
        return handlerOfSweptBlock(block, m_states[id.index()]) || block.isHandlerBlock();
    });
}

ALWAYS_INLINE_INTO_OTHER_FILES void Sweep::needArgumentsOn(const Edge& edge)
{
    std::span<const ValueID> arguments = edge.arguments();
    if (arguments.empty())
        return;
    m_states[edge.from.index()] |= sweptBlockHandsOverArguments;
    m_someBlockHandsOverArguments = true;
    std::span<const ValueID> parameters = m_code->at(edge.to).parameters();
    size_t numberOfPairs = std::min(parameters.size(), arguments.size());
    uint8_t* const marksOfValues = m_arrays.marks;
    for (size_t position = 0; position < numberOfPairs; ++position) {
        ValueID argument = arguments.data()[position];
        if (argument && (marksOfValues[parameters.data()[position].index()] & neededMarkOfSweep))
            needInSweep(marksOfValues, m_worklist, argument);
    }
}

void Sweep::needArgumentsOn(EdgeID id)
{
    needArgumentsOn(m_code->at(id));
}

ALWAYS_INLINE_INTO_OTHER_FILES void Sweep::scanRootsOf(const ArraysByValue& arrays, BlockID id, const Block& block)
{
    RELEASE_ASSERT(block.terminator.isSet);
    for (ValueID parameter : block.parameters()) {
        uint8_t& marks = arrays.marks[parameter.index()];
        if (marks & passedByMarkOfSweep)
            continue;
        marks |= passedByMarkOfSweep;
        if (marks & neededMarkOfSweep)
            m_worklist.append(parameter);
    }
    if (block.handler)
        giveUpHandlerOf(id, block);

    bool conditionIsAskedForAtVisit = block.terminator.kind == TerminatorKind::Branch && !block.isHandlerBlock();
    forEachValueReadAtEndOfSweptBlock(block, [&](ValueID read) {
        if (conditionIsAskedForAtVisit && read != block.subject)
            return;
        needInSweep(arrays.marks, m_worklist, read);
        countUseInSweep(arrays.useCounts, arrays.onlyUsers, read, ValueID());
    });
}

void Sweep::reach(BlockID start)
{
    Code& code = *m_code;
    uint8_t* const states = m_states;
    if (states[start.index()] & sweptBlockIsReached)
        return;
    states[start.index()] |= sweptBlockIsReached;
    code.m_reachedBlocks.quickSet(start.index());

    const ArraysByValue arrays = m_arrays;
    BlockOnSearchPath* const path = m_searchPath.mutableSpan().data();
    uint32_t depth = 0;
    const Block* from = &code.at(start);
    uint32_t indexOfFrom = start.index();
    scanRootsOf(arrays, start, *from);
    uint32_t nextEdge = 0;
    for (;;) {
        uint32_t numberOfEdges = from->terminator.numberOfEdges;
        uint32_t firstEdge = from->terminator.firstEdge.index();
        const Block* below = nullptr;
        uint32_t indexOfBelow = 0;
        while (nextEdge < numberOfEdges) {
            const Edge& edge = code.at(EdgeID(firstEdge + nextEdge++));
            BlockID to = edge.to;
            RELEASE_ASSERT(to);
            needArgumentsOn(edge);
            if (states[to.index()] & sweptBlockIsReached)
                continue;
            states[to.index()] |= sweptBlockIsReached;
            code.m_reachedBlocks.quickSet(to.index());
            const Block& entered = code.at(to);
            scanRootsOf(arrays, to, entered);
            if (entered.terminator.numberOfEdges) {
                below = &entered;
                indexOfBelow = to.index();
                break;
            }
            m_reachedInPostorder.append(to);
        }
        if (below) {
            path[depth++] = { from, indexOfFrom, nextEdge };
            from = below;
            indexOfFrom = indexOfBelow;
            nextEdge = 0;
            continue;
        }
        m_reachedInPostorder.append(BlockID(indexOfFrom));
        if (!depth)
            return;
        BlockOnSearchPath above = path[--depth];
        from = above.block;
        indexOfFrom = above.index;
        nextEdge = above.nextEdge;
    }
}

void Sweep::need(ValueID value)
{
    needInSweep(m_arrays.marks, m_worklist, value);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Sweep::needArgumentsFor(ValueID parameter, const Operation& record)
{
    const Code& code = *m_code;
    BlockID id = record.block();
    RELEASE_ASSERT(id);
    const Block& block = code.at(id);
    std::span<const ValueID> parameters = block.parameters();
    RELEASE_ASSERT(!parameters.empty());

    size_t position = parameter.index() - parameters.front().index();
    if (position >= parameters.size() || parameters[position] != parameter)
        position = std::ranges::find(parameters, parameter) - parameters.begin();
    RELEASE_ASSERT(position < parameters.size());

    EdgeID first = block.firstEdgeIn();
    if (!first)
        return;

    auto needArgumentOn = [&](EdgeID incoming, const Edge& edge) ALWAYS_INLINE_LAMBDA {
        if (!isTaken(incoming, edge))
            return;
        if (m_states[edge.from.index()] & (sweptBlockDoesNothing | sweptBranchDecidesNothing)) [[unlikely]]
            noteArgumentNeededFrom(edge.from, id, position);
        std::span<const ValueID> arguments = edge.arguments();
        if (position < arguments.size() && arguments[position])
            needInSweep(m_arrays.marks, m_worklist, arguments[position]);
    };
    if (!m_hasListedIncomingEdges) {
        if (edgesAscendInto(id, block)) {
            for (EdgeID incoming = first; incoming;) {
                const Edge& edge = code.at(incoming);
                needArgumentOn(incoming, edge);
                incoming = edge.nextInto();
            }
            return;
        }
        listIncomingEdges();
    }
    for (uint32_t index = m_firstIncomingEdge[id.index()]; index < m_firstIncomingEdge[id.index() + 1]; ++index)
        needArgumentOn(m_incomingEdges[index], code.at(m_incomingEdges[index]));
}

static ALWAYS_INLINE bool staysInSweptCodeByItselfByRow(const Operation& operation, uint8_t row)
{
    if (row & isKeptForOperandInRowOfSweep)
        return false;
    if (row & isNeverDroppedInRowOfSweep)
        return true;
    EffectTraits traits = operation.traits();
    if ((row & writesOnlyItsAnswerInRowOfSweep) && !traits.mayThrowByRule() && traits.alwaysCompletes())
        return false;
    if (!neitherThrowsNorRunsCode(traits))
        return true;
    return row & hasNoResultInRowOfSweep;
}

bool Sweep::isRoot(const Operation& operation) const
{
    return staysInSweptCodeByItself(operation);
}

bool staysInSweptCodeByItself(const Operation& operation)
{
    return staysInSweptCodeByItselfByRow(operation, rowOfOpcodeInSweep(operation.opcode));
}

ALWAYS_INLINE_INTO_OTHER_FILES void Sweep::noteNeededOperation(const ArraysByValue& arrays, const uint8_t& stateOfBlock, ValueID value, const Operation& operation)
{
    bool standsBesidePoll = (arrays.marks[value.index()] & leavesOwedErrorMarkOfSweep) && operation.opcode != Opcode::LoopHint;
    if (standsBesidePoll) [[unlikely]]
        keepPollsUnderHandlersBeside(value);
    if ((stateOfBlock & (sweptBlockHasGivenUpHandler | sweptBlockNeedsNoHandler)) == sweptBlockHasGivenUpHandler && (standsBesidePoll || !(arrays.marks[value.index()] & leavesOwedErrorMarkOfSweep)))
        keepHandlerIfThrows(operation);
    if (m_holdsBackStores)
        needStoresHeldBackFor(value);
    forEachOperandNeededBySweep(operation, [&](ValueID operand) {
        needInSweep(arrays.marks, m_worklist, operand);
        countUseInSweep(arrays.useCounts, arrays.onlyUsers, operand, value);
    });
    if (m_followsLiveness)
        noteNeededOperationForLiveness(value, operation);
}

ALWAYS_INLINE_INTO_OTHER_FILES EdgeID Sweep::lastEdgeOfWayThatDoesNothing(EdgeID first) const
{
    const Code& code = *m_code;
    EdgeID last = first;
    for (BlockID to = code.at(last).to; m_states[to.index()] & sweptBlockDoesNothing; to = code.at(last).to)
        last = code.at(to).terminator.edge(0);
    return last;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Sweep::waysOfBranchMeetHavingDoneNothing(const Terminator& branch) const
{
    const Code& code = *m_code;
    const Edge& first = code.at(lastEdgeOfWayThatDoesNothing(branch.taken()));
    const Edge& second = code.at(lastEdgeOfWayThatDoesNothing(branch.notTaken()));
    if (first.to != second.to)
        return false;
    std::span<const ValueID> parameters = code.at(first.to).parameters();
    if (first.arguments().size() != parameters.size() || second.arguments().size() != parameters.size())
        return false;
    for (size_t position = 0; position < parameters.size(); ++position) {
        if ((m_arrays.marks[parameters[position].index()] & neededMarkOfSweep) && first.arguments()[position] != second.arguments()[position])
            return false;
    }
    return true;
}

void Sweep::needConditionOfBranch(BlockID id)
{
    if (m_states[id.index()] & sweptBlockDoesNothing)
        noteThatBlockDoesSomething(id);
    m_states[id.index()] &= static_cast<uint8_t>(~sweptBranchDecidesNothing);
    ValueID condition = m_code->at(id).terminator.conditionOperand();
    need(condition);
    countUse(condition, ValueID());
}

BlockID Sweep::branchAboveWayThatDoesNothing(BlockID block) const
{
    do
        block = m_code->at(m_code->at(block).firstEdgeIn()).from;
    while ((m_states[block.index()] & (sweptBlockDoesNothing | sweptBranchDecidesNothing)) == sweptBlockDoesNothing);
    return block;
}

void Sweep::noteThatBlockDoesSomething(BlockID block)
{
    BlockID above = branchAboveWayThatDoesNothing(block);
    m_states[block.index()] &= static_cast<uint8_t>(~sweptBlockDoesNothing);
    if (m_states[above.index()] & sweptBranchDecidesNothing)
        needConditionOfBranch(above);
}

void Sweep::noteArgumentNeededFrom(BlockID from, BlockID entered, size_t position)
{
    const Code& code = *m_code;
    BlockID above = m_states[from.index()] & sweptBranchDecidesNothing ? from : branchAboveWayThatDoesNothing(from);
    if (!(m_states[above.index()] & sweptBranchDecidesNothing))
        return;
    const Terminator& branch = code.at(above).terminator;
    const Edge& first = code.at(lastEdgeOfWayThatDoesNothing(branch.taken()));
    if (first.to == entered && first.arguments()[position] != code.at(lastEdgeOfWayThatDoesNothing(branch.notTaken())).arguments()[position])
        needConditionOfBranch(above);
}

void Sweep::leadJumpPastBlocksThatHoldNothing(BlockID id)
{
    Code& code = *m_code;
    for (;;) {
        BlockID to = code.at(code.at(id).terminator.next()).to;
        const Block& block = code.at(to);
        if (block.m_numberInOrder || block.terminator.kind != TerminatorKind::Jump || block.subject || block.isEntry || block.isLoopHead || block.isHandlerBlock())
            return;
        for (ValueID parameter : block.parameters()) {
            if (code.isNeeded(parameter))
                return;
        }
        for (EdgeID incoming = block.firstEdgeIn(); incoming; incoming = code.at(incoming).nextInto()) {
            if (code.at(incoming).from != id && isTaken(incoming, code.at(incoming)))
                return;
        }
        code.leadJumpPastBlockThatJumps(id);
        m_states[to.index()] = 0;
        code.m_reachedBlocks.quickClear(to.index());
    }
}

void Sweep::turnBranchesThatDecideNothingIntoJumps()
{
    Code& code = *m_code;
    for (BlockID id : m_branchesThatDecideNothing) {
        if (!(m_states[id.index()] & sweptBranchDecidesNothing))
            continue;
        const Terminator& branch = code.at(id).terminator;
        EdgeID lastOnWay = lastEdgeOfWayThatDoesNothing(branch.taken());
        for (EdgeID first : { branch.taken(), branch.notTaken() }) {
            for (BlockID block = code.at(first).to; m_states[block.index()] & sweptBlockDoesNothing; block = code.at(code.at(block).terminator.next()).to) {
                m_states[block.index()] = 0;
                code.m_reachedBlocks.quickClear(block.index());
            }
        }
        code.turnBranchIntoJump(id, lastOnWay);
        leadJumpPastBlocksThatHoldNothing(id);
        if (!code.at(code.at(id).terminator.next()).arguments().empty())
            m_states[id.index()] |= sweptBlockHandsOverArguments;
        ++m_totals.branchesThatDecideNothing;
    }
    if (m_totals.branchesThatDecideNothing) {
        m_reachedInPostorder.removeAllMatching([&](BlockID id) {
            return !(m_states[id.index()] & sweptBlockIsReached);
        });
    }
}

void Sweep::misfoldBranch()
{
    Code& code = *m_code;
    for (BlockID id : m_reachedInPostorder) {
        const Terminator& end = code.at(id).terminator;
        if (end.kind != TerminatorKind::Branch || code.at(id).isHandlerBlock())
            continue;
        BlockID other = code.at(end.notTaken()).to;
        if (code.at(other).isLoopHead)
            continue;
        uint32_t numberOfWaysIn = 0;
        for (EdgeID incoming = code.at(other).firstEdgeIn(); incoming; incoming = code.at(incoming).nextInto())
            numberOfWaysIn += code.at(incoming).to == other && isTaken(incoming, code.at(incoming));
        if (numberOfWaysIn < 2)
            continue;
        code.turnBranchIntoJump(id, end.taken());
        if (!code.at(code.at(id).terminator.next()).arguments().empty())
            m_states[id.index()] |= sweptBlockHandsOverArguments;
        return;
    }
}

void Sweep::dropTakenEdge()
{
    Code& code = *m_code;
    for (BlockID id : m_reachedInPostorder) {
        if (code.at(id).terminator.kind != TerminatorKind::Jump)
            continue;
        code.m_edges[code.at(id).terminator.next().index()].to = BlockID();
        return;
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES void Sweep::visitOperationsOf(BlockID id)
{
    Code& code = *m_code;
    Block& block = code.m_blocks[id.index()];
    const ArraysByValue arrays = m_arrays;
    if (block.terminator.kind == TerminatorKind::Branch && !block.isHandlerBlock()) {
        if (waysOfBranchMeetHavingDoneNothing(block.terminator)) [[unlikely]] {
            m_states[id.index()] |= sweptBranchDecidesNothing;
            m_branchesThatDecideNothing.append(id);
        } else {
            ValueID condition = block.terminator.conditionOperand();
            needInSweep(arrays.marks, m_worklist, condition);
            countUseInSweep(arrays.useCounts, arrays.onlyUsers, condition, ValueID());
        }
    }
    const uint8_t& stateOfBlock = m_states[id.index()];
    ValueID* const order = code.m_order.mutableSpan().data();
    const uint32_t endInOrder = m_firstInOrderSoFar;
    uint32_t firstInOrder = endInOrder;

    const bool looksForCoveredBarriers = m_looksForCoveredBarriers;
    const bool looksForCoveredFences = m_looksForCoveredFences;
    if (looksForCoveredBarriers && !m_coveredAtHeads.isEmpty() && block.terminator.numberOfEdges && m_numberCoveredAtHead[code.at(block.terminator.edge(0)).to.index()])
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

        uint8_t row = rowOfOpcodeInSweep(opcode);
        uint8_t& marks = arrays.marks[value.index()];
        marks |= passedByMarkOfSweep;
        bool isNeeded = marks & neededMarkOfSweep;
        if (!isNeeded) {
            if (row & isKeptForOperandInRowOfSweep) {
                if (!noteOperationKeptForItsOperand(id, value, operation))
                    continue;
            } else {
                if ((marks & pollsForNothingMarkOfSweep) || !staysInSweptCodeByItselfByRow(operation, row))
                    continue;
                if (opcode == Opcode::CheckProof && operation.goesWithItsValue()) {
                    ValueID asked = operation.valueOperand();
                    const Operation& made = code.at(asked);
                    if (!(arrays.marks[asked.index()] & neededMarkOfSweep) && !staysInSweptCodeByItselfByRow(made, rowOfOpcodeInSweep(made.opcode))) {
                        holdBackUntilNeeded(value, asked);
                        ++m_totals.doorsDroppedWithTheirValues;
                        continue;
                    }
                }
                EffectTraits traits = operation.traits();
                ValueID object;
                if (!traits.mayBeTerminated() && !traits.mayRunOtherCode() && !traits.mayWriteOutsideObjects() && isStoreIntoOwnObject(operation, object) && !(arrays.marks[object.index()] & neededMarkOfSweep)) {
                    holdBackUntilNeeded(value, object);
                    continue;
                }
                marks |= neededMarkOfSweep;
                isNeeded = true;
            }
        }
        order[--firstInOrder] = value;
        if (isNeeded)
            noteNeededOperation(arrays, stateOfBlock, value, operation);
        if (looksForCoveredBarriers) {
            bool hasOwnerBarrier = row & hasOwnerBarrierInRowOfSweep;
            if (hasOwnerBarrier || !m_coveredOwners.isEmpty())
                lookForCoveredBarrier(value, operation);
            if (hasOwnerBarrier || !m_storesThatAwaitBirth.isEmpty())
                lookForBirthOfOwner(value, operation);
        }
        if (opcode == Opcode::CreateUnmappedArguments && m_looksForBirthsToMove)
            noteIfBirthMayMove(value, operation);
        if (looksForCoveredFences && (m_laterFenceCovers || (row & isAllocationWithFenceInRowOfSweep)))
            lookForCoveredFence(value, operation);
    }
    m_totals.operationsSwept += numberOfOperationsVisited;
    m_totals.operationsDropped += numberOfOperationsVisited;
    if (looksForCoveredBarriers && !m_coveredOwners.isEmpty())
        endCoverageAtHeadOf(id);
    if (!m_storesThatAwaitBirth.isEmpty())
        forgetStoresThatAwaitBirth();
    if (looksForCoveredFences && m_laterFenceCovers)
        endFenceCoverageAtHeadOf(id);
    uint32_t numberInOrder = endInOrder - firstInOrder;
    bool endsWithoutDeciding = block.terminator.kind == TerminatorKind::Jump || ((stateOfBlock & sweptBranchDecidesNothing) && code.at(lastEdgeOfWayThatDoesNothing(block.terminator.taken())).arguments().empty());
    if (!numberInOrder && endsWithoutDeciding && !block.m_numberOfParameters && !block.subject && !block.isEntry && !block.isHandlerBlock() && block.m_firstEdgeIn && block.m_firstEdgeIn == block.m_lastEdgeIn)
        m_states[id.index()] |= sweptBlockDoesNothing;
    block.m_firstInOrder = firstInOrder;
    block.m_numberInOrder = numberInOrder;
    m_firstInOrderSoFar = firstInOrder;
    m_numberOfOperationsInOrder += numberInOrder;
}

void Sweep::sweepBackward()
{
    Code& code = *m_code;
    uint32_t numberOfValues = code.numberOfValues();
    uint32_t numberOfBlocks = code.numberOfBlocks();
    code.m_order.fill(ValueID(), numberOfValues);
    m_firstInOrderSoFar = numberOfValues;
    code.m_useCounts.fill(0, numberOfValues);
    code.m_onlyUsers.fill(ValueID(), numberOfValues);
    m_arrays = { code.m_marks.mutableSpan().data(), code.m_useCounts.mutableSpan().data(), code.m_onlyUsers.mutableSpan().data() };
    m_statesOfBlocks.fill(0, numberOfBlocks);
    m_states = m_statesOfBlocks.mutableSpan().data();
    if (m_searchPath.size() < numberOfBlocks)
        m_searchPath.grow(numberOfBlocks);
    m_looksForCoveredBarriers = beginLookingForCoveredBarriers();
    m_looksForCoveredFences = beginLookingForCoveredFences();
    m_lendsClosures = beginLendingClosures();
    m_lendsCells = beginLendingCells();
    m_looksForBirthsToMove = !m_sweepsAgainBehindMovedBirths && beginLookingForBirthsToMove();
    beginFollowingLiveness();

    for (const EntryOfCode& entry : code.entries())
        reach(entry.block);
    size_t next = 0;
    do {
        if (!m_worklist.isEmpty() || !m_blocksToReach.isEmpty())
            settleWhatIsNeeded();
        for (; next < m_reachedInPostorder.size(); ++next) {
            visitOperationsOf(m_reachedInPostorder[next]);
            if (!m_worklist.isEmpty() || !m_blocksToReach.isEmpty())
                settleWhatIsNeeded();
        }
        needOperandsOfWhatWayDoesNotComeBy();
    } while (!m_worklist.isEmpty());
    for (uint8_t& marks : code.m_marks)
        marks &= static_cast<uint8_t>(~passedByMarkOfSweep);
    if (m_reachedInPostorder.size() != numberOfBlocks) {
        for (uint32_t index = 0; index < numberOfBlocks; ++index) {
            if (m_states[index] & sweptBlockIsReached)
                continue;
            code.m_blocks[index].m_firstInOrder = 0;
            code.m_blocks[index].m_numberInOrder = 0;
        }
    }
    if (code.m_hasHandlers)
        forgetHandlersThatAreGone();

    if (!m_operationsKeptForOperands.isEmpty())
        settleOperationsKeptForTheirOperands();
    if (!m_operationsNeededLate.isEmpty())
        putOperationsNeededLateInOrder();
    if (!m_branchesThatDecideNothing.isEmpty())
        turnBranchesThatDecideNothingIntoJumps();
    if (m_inputs->shouldMisfoldBranch) [[unlikely]]
        misfoldBranch();
    m_totals.operationsDropped -= m_numberOfOperationsInOrder;
    code.m_numberOfOperationsInOrder = m_numberOfOperationsInOrder;
    countUsesAtEndsOfBlocks();
    m_looksForBirthsToMove = false;
    if (!m_birthsThatMayMove.isEmpty() && moveBirthsToWhereCellsAreNeeded())
        return;
    if (m_lendsClosures)
        lendClosures();
    if (m_lendsCells)
        lendCells();
    if (m_coverageIsOutOfDate)
        lookForCoverageAgain();
    sortFactUses();
    if (m_followsLiveness)
        finishLiveness();
}

void Sweep::settleWhatIsNeeded()
{
    Code& code = *m_code;
    const ArraysByValue arrays = m_arrays;
    for (;;) {
        while (!m_blocksToReach.isEmpty())
            reach(m_blocksToReach.takeLast());
        if (m_worklist.isEmpty())
            return;

        ValueID value = m_worklist.takeLast();
        const Operation& operation = code.at(value);
        if (operation.opcode == Opcode::Parameter) {
            if (m_holdsBackStores)
                needStoresHeldBackFor(value);
            needArgumentsFor(value, operation);
            continue;
        }
        ++m_totals.operationsSwept;
        if (m_states[operation.blockIndex] & sweptBlockDoesNothing) [[unlikely]]
            noteThatBlockDoesSomething(operation.block());
        if (!mayBeKeptForItsOperand(operation))
            m_operationsNeededLate.append({ operation.block(), value });
        if (operation.opcode == Opcode::CheckProof)
            --m_totals.doorsDroppedWithTheirValues;
        noteNeededOperation(arrays, m_states[operation.blockIndex], value, operation);
        m_coverageIsOutOfDate = m_coverageIsOutOfDate || mayChangeWhatIsCovered(operation);
        if (m_lendsClosures && operation.opcode == Opcode::NewClosure)
            m_closuresThatMayBeLent.append(value);
        if (m_lendsCells && operation.opcode == Opcode::NewObject)
            m_cellsThatMayBeLent.append(value);
    }
}

bool Sweep::mayBeKeptForItsOperand(const Operation& operation) const
{
    if (!operationsThatSweepKeepsForTheirOperand.contains(operation.opcode))
        return false;
    ValueID operand = operation.operand(OperandIndex::ReleaseEnvironment::environment);
    return !m_code->isConstant(operand) && holdsCells(m_code->at(operand).representation);
}

bool Sweep::noteOperationKeptForItsOperand(BlockID block, ValueID id, const Operation& operation)
{
    if (!mayBeKeptForItsOperand(operation))
        return false;
    m_operationsKeptForOperands.append({ block, id, operation.operand(OperandIndex::ReleaseEnvironment::environment), operation.opcode });
    return true;
}

void Sweep::settleOperationsKeptForTheirOperands()
{
    Code& code = *m_code;
    Vector<BlockID, 8> blocksWithGaps;
    for (const OperationKeptForOperand& kept : m_operationsKeptForOperands) {
        if (code.isNeeded(kept.operation))
            continue;
        if (code.isNeeded(kept.operand)) {
            code.m_marks[kept.operation.index()] |= neededMarkOfSweep;
            countUse(kept.operand, kept.operation);
            if (m_followsLiveness && kept.opcode != Opcode::ReleaseEnvironment) {
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

void Sweep::needOperandsOfWhatWayDoesNotComeBy()
{
    Code& code = *m_code;
    ASSERT(m_worklist.isEmpty());
    Vector<ValueID, 16> notComeBy;
    std::span<const uint8_t> marks = code.m_marks.span();
    auto listIfMadeOfOthers = [&](size_t index) ALWAYS_INLINE_LAMBDA {
        if (code.at(ValueID(index)).numberOfOperands())
            notComeBy.append(ValueID(index));
    };
    constexpr size_t marksInWord = sizeof(uint64_t);
    constexpr uint64_t neededMarkOfEach = 0x0101010101010101ull * neededMarkOfSweep;
    static_assert(neededMarkOfSweep == 1 && passedByMarkOfSweep == 1 << 6 && std::endian::native == std::endian::little);
    size_t index = 0;
    for (; index + marksInWord <= marks.size(); index += marksInWord) {
        uint64_t word;
        memcpy(&word, marks.data() + index, marksInWord);
        for (uint64_t found = word & ~(word >> 6) & neededMarkOfEach; found; found &= found - 1)
            listIfMadeOfOthers(index + std::countr_zero(found) / 8);
    }
    for (; index < marks.size(); ++index) {
        if ((marks[index] & (neededMarkOfSweep | passedByMarkOfSweep)) == neededMarkOfSweep)
            listIfMadeOfOthers(index);
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

void Sweep::countUse(ValueID used, ValueID user)
{
    countUseInSweep(m_arrays.useCounts, m_arrays.onlyUsers, used, user);
}

void Sweep::countUsesAtEndsOfBlocks()
{
    Code& code = *m_code;
    bool notesParametersFed = code.hasLoops();
    code.m_parametersFed.fill(ValueID(), notesParametersFed ? code.numberOfValues() : 0);
    if (!m_someBlockHandsOverArguments)
        return;

    const ArraysByValue arrays = m_arrays;
    const uint8_t* const states = m_states;
    ValueID* const parametersFed = code.m_parametersFed.mutableSpan().data();
    for (BlockID id : m_reachedInPostorder) {
        forEachArgumentCountedBySweepAtEndOf(code, arrays.marks, states[id.index()], code.at(id), [&](ValueID argument, ValueID parameter) ALWAYS_INLINE_LAMBDA {
            countUseInSweep(arrays.useCounts, arrays.onlyUsers, argument, ValueID());
            if (!notesParametersFed)
                return;
            ValueID& fed = parametersFed[argument.index()];
            if (!fed || parameter < argument)
                fed = parameter;
        });
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
