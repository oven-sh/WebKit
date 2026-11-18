/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTSweepLiveness.h"
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class Plan;
class Program;
struct PlannedLayout;

struct SweepInputs {
    const Program& program;
    const Facts& facts;
    const Plan& plan;
    TypeArena& types;
    bool shouldMisfoldBranch { false };
    bool shouldDropEdge { false };
};

struct ReadOfSweptValue {
    BlockID block;
    ValueID reader;
    uint32_t previous { UINT32_MAX };
};

struct SweepTotals {
    uint32_t operationsSwept { 0 };
    uint32_t operationsDropped { 0 };
    uint32_t branchesThatDecideNothing { 0 };
    uint32_t doorsDroppedWithTheirValues { 0 };
    uint32_t overwrittenSlotStoresDropped { 0 };
    uint32_t allocationsSunk { 0 };
};

inline constexpr uint8_t neededMarkOfSweep = static_cast<uint8_t>(SweepMark::IsNeeded);
inline constexpr uint8_t passedByMarkOfSweep = 1 << 6;

struct RoomsOfSweep;
class SlotStoresOfBlock;

class Sweep {
    WTF_MAKE_NONCOPYABLE(Sweep);
    friend struct RoomsOfSweep;
public:
    explicit Sweep(RoomsOfSweep&);

    SweepTotals run(const SweepInputs&, Code&);

private:
    struct ArraysByValue {
        uint8_t* marks;
        uint8_t* useCounts;
        ValueID* onlyUsers;
    };

    void listIncomingEdges();
    void listEdgesThatAreTaken();
    void ensureEdgesThatAreTakenAreListed();
    bool edgesAscendInto(BlockID, const Block&);
    void reach(BlockID);
    void scanRootsOf(const ArraysByValue&, BlockID, const Block&);
    void need(ValueID);
    void needArgumentsFor(ValueID parameter, const Operation&);
    void needArgumentsOn(EdgeID);
    void needArgumentsOn(const Edge&);
    bool isTaken(EdgeID, const Edge&) const;
    bool isRoot(const Operation&) const;
    void forgetEdgesThatAreNotTaken();
    void forgetHandlersThatAreGone();
    void sortFactUses();
    void sweepBackward();
    void visitOperationsOf(BlockID);
    void noteNeededOperation(const ArraysByValue&, const uint8_t& stateOfBlock, ValueID, const Operation&);
    void settleWhatIsNeeded();
    bool mayBeKeptForItsOperand(const Operation&) const;
    bool noteOperationKeptForItsOperand(BlockID, ValueID, const Operation&);
    void settleOperationsKeptForTheirOperands();
    void putOperationsNeededLateInOrder();
    void needOperandsOfWhatWayDoesNotComeBy();
    void countUse(ValueID used, ValueID user);
    void countUsesAtEndsOfBlocks();

    EdgeID lastEdgeOfWayThatDoesNothing(EdgeID first) const;
    bool waysOfBranchMeetHavingDoneNothing(const Terminator&) const;
    void needConditionOfBranch(BlockID);
    BlockID branchAboveWayThatDoesNothing(BlockID) const;
    void noteThatBlockDoesSomething(BlockID);
    void noteArgumentNeededFrom(BlockID, BlockID entered, size_t position);
    void turnBranchesThatDecideNothingIntoJumps();
    void misfoldBranch();
    void dropTakenEdge();
    void leadJumpPastBlocksThatHoldNothing(BlockID);

    bool isStoreIntoOwnObject(const Operation&, ValueID& object);
    void holdBackUntilNeeded(ValueID store, ValueID object);
    void needStoresHeldBackFor(ValueID object);

    void giveUpHandlerOf(BlockID, const Block&);
    void keepHandlerOf(BlockID);
    void keepHandlerIfThrows(const Operation& needed);
    void keepPollsUnderHandlersBeside(ValueID kept);
    void settlePollsThatLeaveUnlessRecordsAreKept();
    Vector<PollWithFate, 4> m_pollsThatLeaveUnlessKept;
    bool handlerOnlyThrowsOutOfUnit(BlockID handler);
    bool beginLookingForCoveredBarriers();
    void beginCoverageAtEndOf(BlockID);
    void lookForCoveredBarrier(ValueID, const Operation&);
    void endCoverageAtHeadOf(BlockID);
    void strikeCoveredOwner(ValueID);
    void strikeCoveredOwners();
    void lookForBirthOfOwner(ValueID, const Operation&);
    void forgetStoresThatAwaitBirth();
    void awaitBirthsAheadOf(BlockID);
    bool beginLookingForCoveredFences();
    void beginFenceCoverageAtEndOf(BlockID);
    void lookForCoveredFence(ValueID, const Operation&);
    void endFenceCoverageAtHeadOf(BlockID);
    bool mayChangeWhatIsCovered(const Operation&) const;
    void lookForCoverageAgain();

    bool beginLendingClosures();
    bool noteIfClosureMayBeLent(ValueID);
    void lendClosures();
    bool beginLendingCells();
    bool noteIfCellMayBeLent(ValueID);
    void lendCells();

    bool beginLookingForBirthsToMove();
    void noteIfBirthMayMove(ValueID, const Operation&);
    bool canMoveAllocation(ValueID, const Operation&) const;
    bool moveBirthsToWhereCellsAreNeeded();
    SweepTotals sweepAgainBehindMovedBirths(const SweepInputs&, Code&);

    void beginFollowingLiveness();
    void noteNeededOperationForLiveness(ValueID, const Operation&);
    void noteReadForLiveness(ValueID read, BlockID, ValueID reader);
    void followIfEnvironmentIsHandedBack(ValueID needed);
    void noteIfMayHoldEnvironmentInFrame(ValueID needed);
    void followCellsThatHoldEnvironmentsInFrames();
    void finishLiveness();
    void findLiveSets(std::span<const ValueID> releaseHints, const SweepBits* valuesToFollow);
    SweepValueSets::Set liveThroughLoopsAround(BlockID) const;
    void markReleasesBehindLastUsesBySets();
    void listLiveValuesAtSuspensionsBySets();

    const SweepInputs* m_inputs { nullptr };
    Code* m_code { nullptr };
    SweepTotals m_totals;
    ArraysByValue m_arrays { };
    RoomsOfSweep& m_rooms;
    Vector<uint8_t>& m_statesOfBlocks;
    uint8_t* m_states { nullptr };
    Vector<ValueID>& m_worklist;
    Vector<BlockID>& m_blocksToReach;
    Vector<uint32_t>& m_firstIncomingEdge;
    Vector<EdgeID>& m_incomingEdges;
    bool m_hasListedIncomingEdges { false };
    bool m_hasForgottenEdgesThatAreNotTaken { false };
    bool m_someBlockHandsOverArguments { false };
    BlockID m_blockThatEdgesAscendInto;
    struct BlockOnSearchPath {
        const Block* block;
        uint32_t index;
        uint32_t nextEdge;
    };
    Vector<BlockOnSearchPath>& m_searchPath;
    Vector<BlockID>& m_reachedInPostorder;
    Vector<BlockID>& m_branchesThatDecideNothing;
    SweepBits& m_heldBack;
    bool m_holdsBackStores { false };
    std::array<uint8_t, numberOfEnumerators<Opcode>>& m_operandsWrittenByHeldBackStores;
    std::array<uint8_t, numberOfEnumerators<Opcode>>& m_opcodesOfAllocationsWhoseStoresAreHeldBack;
    Vector<ValueID>& m_waitingStores;

    struct OperationInBlock {
        BlockID block;
        ValueID operation;
    };
    Vector<OperationInBlock>& m_operationsNeededLate;
    struct OperationKeptForOperand {
        BlockID block;
        ValueID operation;
        ValueID operand;
        Opcode opcode { Opcode::Undefined };
    };
    Vector<OperationKeptForOperand>& m_operationsKeptForOperands;
    uint32_t m_numberOfOperationsInOrder { 0 };
    uint32_t m_firstInOrderSoFar { 0 };
    bool m_looksForCoveredBarriers { false };
    bool m_looksForCoveredFences { false };
    bool m_coverageIsOutOfDate { false };
    bool m_lendsClosures { false };
    Vector<ValueID>& m_closuresThatMayBeLent;
    bool m_lendsCells { false };
    Vector<ValueID>& m_cellsThatMayBeLent;
    bool m_looksForBirthsToMove { false };
    bool m_hasMovedBirths { false };
    bool m_sweepsAgainBehindMovedBirths { false };
    Vector<ValueID, 4> m_overwrittenStores;
    struct StoreDroppedUnlessRead {
        ValueID store;
        ValueID read;
    };
    Vector<StoreDroppedUnlessRead> m_storesDroppedUnlessRead;
    bool isOverwrittenUnread(ValueID store, const Operation&, SlotStoresOfBlock&);
    void needStoresReadLate();
    Vector<ValueID>& m_birthsThatMayMove;
    struct EnvironmentHandedBack {
        LayoutID layout;
        ValueID value;
    };
    Vector<EnvironmentHandedBack>& m_environmentsHandedBack;
    Vector<ValueID>& m_namesOfCellsThatMayHoldEnvironments;
    bool m_followsLiveness { false };
    bool m_mayHandBackEnvironments { false };
    bool m_hasInteriorPointers { false };
    SweepBits& m_environmentsFollowed;
    SweepBits& m_blocksThatSuspend;
    SweepBits& m_blocksWithHints;
    Vector<uint32_t>& m_lastReads;
    Vector<ReadOfSweptValue>& m_reads;
    std::unique_ptr<SweepLivenessStorage> m_liveness;
    Vector<ValueID>& m_lastReadersInOwnBlocks;
    Vector<ValueID>& m_coveredAtHeads;
    Vector<uint32_t>& m_firstCoveredAtHead;
    Vector<uint32_t>& m_numberCoveredAtHead;
    Vector<ValueID>& m_coveredOwners;
    SweepBits& m_ownersCovered;
    ValueID m_requiredObject;
    ValueID m_storeWhoseOwnerIsKnown;
    ValueID m_ownerOfThatStore;
    struct StoreThatAwaitsBirth {
        ValueID store;
        ValueID owner;
    };
    Vector<StoreThatAwaitsBirth, 8>& m_storesThatAwaitBirth;
    SweepBits& m_birthsAwaited;
    BlockID m_blockAwaitingAllocations;
    bool m_laterFenceCovers { false };
    bool m_laterFenceCoversAtSomeHead { false };
    SweepBits& m_laterFenceCoversAtHeads;
};

struct RoomsOfSweep {
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_statesOfBlocks)>> roomOfStatesOfBlocks;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_worklist)>> roomOfWorklist;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_blocksToReach)>> roomOfBlocksToReach;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_firstIncomingEdge)>> roomOfFirstIncomingEdge;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_incomingEdges)>> roomOfIncomingEdges;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_searchPath)>> roomOfSearchPath;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_reachedInPostorder)>> roomOfReachedInPostorder;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_branchesThatDecideNothing)>> roomOfBranchesThatDecideNothing;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_heldBack)>> roomOfHeldBack;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_waitingStores)>> roomOfWaitingStores;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_operationsNeededLate)>> roomOfOperationsNeededLate;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_operationsKeptForOperands)>> roomOfOperationsKeptForOperands;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_closuresThatMayBeLent)>> roomOfClosuresThatMayBeLent;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_cellsThatMayBeLent)>> roomOfCellsThatMayBeLent;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_birthsThatMayMove)>> roomOfBirthsThatMayMove;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_environmentsHandedBack)>> roomOfEnvironmentsHandedBack;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_namesOfCellsThatMayHoldEnvironments)>> roomOfNamesOfCellsThatMayHoldEnvironments;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_environmentsFollowed)>> roomOfEnvironmentsFollowed;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_blocksThatSuspend)>> roomOfBlocksThatSuspend;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_blocksWithHints)>> roomOfBlocksWithHints;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_lastReads)>> roomOfLastReads;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_reads)>> roomOfReads;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_lastReadersInOwnBlocks)>> roomOfLastReadersInOwnBlocks;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_coveredAtHeads)>> roomOfCoveredAtHeads;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_firstCoveredAtHead)>> roomOfFirstCoveredAtHead;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_numberCoveredAtHead)>> roomOfNumberCoveredAtHead;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_coveredOwners)>> roomOfCoveredOwners;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_ownersCovered)>> roomOfOwnersCovered;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_storesThatAwaitBirth)>> roomOfStoresThatAwaitBirth;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_birthsAwaited)>> roomOfBirthsAwaited;
    RoomOnly<std::remove_reference_t<decltype(Sweep::m_laterFenceCoversAtHeads)>> roomOfLaterFenceCoversAtHeads;
    std::array<uint8_t, numberOfEnumerators<Opcode>> operandsWrittenByHeldBackStores { };
    std::array<uint8_t, numberOfEnumerators<Opcode>> opcodesOfAllocationsWhoseStoresAreHeldBack { };
};

bool staysInSweptCodeByItself(const Operation&);

bool objectOfLayoutMayLieInFrame(const PlannedLayout&);

NEVER_INLINE inline ValueID cellBehindNames(const Code& code, ValueID name)
{
    return code.valueBehindNames<cellPreservingOpcodes>(name);
}

ALWAYS_INLINE ValueID underlyingCellOf(const Code& code, ValueID cell)
{
    if (!cellPreservingOpcodes.contains(code.at(cell).opcode)) [[likely]]
        return cell;
    return cellBehindNames(code, cell);
}

template<typename Functor>
ALWAYS_INLINE void forEachOperandNeededBySweep(const Operation& operation, const Functor& functor)
{
    if (operation.opcode == Opcode::Narrowed) {
        functor(operation.operand(OperandIndex::Narrowed::value));
        return;
    }
    operation.forEachChild(functor);
}

template<typename Functor>
ALWAYS_INLINE void forEachValueReadAtEndOfSweptBlock(const Block& block, const Functor& functor)
{
    block.terminator.forEachChild(functor);
    if (block.subject)
        functor(block.subject);
}

template<typename Functor>
ALWAYS_INLINE void forEachArgumentCountedBySweepAtEndOf(const Code& code, const uint8_t* marksOfValues, uint8_t stateOfBlock, const Block& block, const Functor& functor)
{
    if (!(stateOfBlock & sweptBlockHandsOverArguments))
        return;
    auto forEachArgumentOn = [&](const Edge& edge) ALWAYS_INLINE_LAMBDA {
        std::span<const ValueID> arguments = edge.arguments();
        if (arguments.empty())
            return;
        std::span<const ValueID> parameters = code.at(edge.to).parameters();
        size_t numberOfPairs = std::min(parameters.size(), arguments.size());
        for (size_t position = 0; position < numberOfPairs; ++position) {
            ValueID argument = arguments.data()[position];
            ValueID parameter = parameters.data()[position];
            if (argument && (marksOfValues[parameter.index()] & neededMarkOfSweep))
                functor(argument, parameter);
        }
    };
    for (uint32_t index = 0; index < block.terminator.numberOfEdges; ++index)
        forEachArgumentOn(code.at(block.terminator.edge(index)));
    if (EdgeID handler = handlerOfSweptBlock(block, stateOfBlock))
        forEachArgumentOn(code.at(handler));
}

inline bool stringBuilderIsCellOfHeap(const Code& code, ValueID builder)
{
    return code.at(underlyingCellOf(code, builder)).opcode == Opcode::StringBuilderBegin;
}

inline constexpr OpcodeSet sweptOperationsWithOwnerBarriers = opcodesWhere([](Opcode opcode) {
    return runsOwnerBarrierOf(opcode) && remembersOwnerOf(opcode);
});
inline constexpr OpcodeSet sweptAllocationsWithFences { Opcode::NewObject, Opcode::NewClosure, Opcode::NewArray, Opcode::CreateRest };
inline constexpr OpcodeSet birthsThatSweepMayMove { Opcode::NewObject, Opcode::NewArray, Opcode::NewClosure, Opcode::CreateRest, Opcode::CreateUnmappedArguments };
inline constexpr OpcodeSet storesThatMoveWithBirthsInSweep { Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::PutEnvironmentField, Opcode::SetMappedArgument, Opcode::FinishBirth, Opcode::TryFinishBirth };

bool newObjectFencesBeforeHandingCellOn(const Program&, const Plan&, const Operation& newObject);
bool newArrayFencesBeforeHandingCellOn(const Operation& newArray);

bool stubOfMethodEntersFunctionsThatOnlyCallFirstArgument(const Program&, const Facts&, BuiltinID method);
bool unitMayLendWhatItMakes(const Program&, CodeUnit);
String findMarkOfSweepThatNothingCovers(const SweepInputs&, Code&);
bool closuresOfFunctionMayBeLent(const Program&, FunctionID);
bool closureHasCellThatMayBeLent(const Program&, ClosureForm, FunctionID);
bool functionEnteredOnlyCallsWhatArrivesAt(const Facts&, CodeRole, uint32_t numberOfBoundArguments, FunctionID, uint32_t position);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
